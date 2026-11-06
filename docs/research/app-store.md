# App Store sui port di iOS per hardware non Apple: cosa risulta, e cosa significa per `andromeda`

Ricerca del 6 novembre 2026, solo lettura: nessun comando sul telefono, nessun codice dei repository scaricati eseguito.
Le affermazioni sugli altri progetti vengono dai file, dai commit e dalle pagine elencati in [Fonti](#9-fonti). Dove una
cosa è solo dichiarata (testo senza foto, log o codice), è segnalato. Questo documento descrive cosa altri hanno
osservato; **non è una guida per aggirare protezioni né per far passare il telefono per un iPhone**, cose che il
progetto non fa (vedi [CONTRIBUTING.md](../../CONTRIBUTING.md) e la sezione 8).

## Sommario

- **Nessun progetto noto ha caricato i contenuti dell'App Store, fatto l'accesso con un Apple ID nel negozio, scaricato
  o installato un'app dal negozio, né eseguito un'app cifrata con FairPlay** su hardware non Apple. Lo stesso vale per
  Corellium, che virtualizza iPhone per mestiere.
- **Quello che il proprietario ricorda di aver visto è quasi certamente HakiOS.** Il 19–20 ottobre 2026, su un ramo
  sperimentale, HakiOS (OOSEX GANG, autore **vcvkk**) ha portato l'App Store di iOS 16.5 sul POCO X5 Pro ad **aprirsi e
  disegnare la propria interfaccia** (carta «Welcome to the App Store», scelta sulla pubblicità, barra schede Today /
  Games / Apps / Arcade / Search), via Wi-Fi, senza Apple ID, SIM né attivazione. Ci sono **tre screenshot** (commit
  `5154263239`), e l'immagine `docs/appstore-tabs.png` è ancora nel README di centinaia di fork. **La scheda Today non
  si è mai caricata** («Cannot Connect to App Store»). Il 26 ottobre OOSEX GANG ha **rimosso** questi esperimenti dal
  repository pubblico (commit `698b648cb7`), perché toccavano i processi del negozio e del DRM di Apple e facevano
  richieste ai suoi server.
- **Anche il sito di ios-sweet scrive «The App Store opens»** (installazione non provata, mancava Internet). È una frase
  sul sito, senza foto né log; il README dello stesso progetto dice «There is no App Store».
- **IOS-26-Hot (iOS 26.5.2 su SM8150) dichiara «App Store & 3rd-Party Apps: Working», ma non è l'App Store di Apple.**
  È un negozio proprio, *HotStore*, che mostra un catalogo tramite le API pubbliche di ricerca di iTunes e installa solo
  `.ipa` **non cifrate**; quelle cifrate (`cryptid = 1`) le rifiuta con un messaggio esplicito.
- **Chi fa girare app di terzi usa solo binari non cifrati** (decifrati altrove, open source o compilati da sé):
  ios-flame (YouTube), IOS-26-Hot, ios-apollo, il port del Redmi K90 (`annibale`), danhol86/ios-sweet, Bergamota. Il
  motivo è nel codice aperto di XNU (sezione 3): un'app con `cryptid = 1` viene caricata solo se il modulo FairPlay di
  Apple ha registrato il suo decifratore; quel modulo esiste solo in forma binaria e dipende dall'hardware Apple.
- **Per noi**: Internet via USB, poi installazione di app **non cifrate**. È fattibile e riusa lavoro con licenza
  MIT/APSL. Contenuti del negozio, accesso Apple ID, download e app cifrate oggi non si fanno. La riga del
  [ROADMAP](../../ROADMAP.md) («non realizzabile») resta valida; va solo precisata: l'**interfaccia** del negozio può
  aprirsi, il **negozio** (catalogo, acquisti, download) no.

---

## 1. Cosa vuol dire «l'App Store funziona»: i livelli

| Livello | Cosa | Chi ci è arrivato | Prova |
|---|---|---|---|
| L0 | L'icona c'è, l'app si apre, mostra «LOADING» | ios-sweet (sito), Arming1/ios-sweet (icona a schermo), HakiOS (19/10) | testo; log HakiOS |
| L1 | Onboarding e interfaccia del negozio disegnati (configurazione «bag» e interfaccia «jetpack» scaricate da Apple) | **solo HakiOS** (20/10) | 3 screenshot, log, commit |
| L2 | Contenuti del negozio (Today, ricerca, pagine app) | **nessuno**: HakiOS si ferma a «Cannot Connect to App Store» | — |
| L3 | Accesso Apple ID **nel negozio** | **nessuno** | — |
| L4 | Acquisto / download dal negozio | **nessuno** su hardware non Apple | — |
| L5 | Installazione via `appstored`/`installd` | dal negozio nessuno (IOS-26-Hot ha un installatore proprio per `.ipa`) | codice |
| L6 | Esecuzione di app cifrate FairPlay (`cryptid = 1`) | **nessuno**, nemmeno Corellium | — |
| a parte | App **non cifrate** copiate nel filesystem (sideload) | ios-flame, IOS-26-Hot, ios-apollo, K90, danhol86, Bergamota | log, codice, testo |

---

## 2. Progetto per progetto

### 2.1 HakiOS (OOSEX GANG) — arriva a L1, poi rimosso dal pubblico

- **Hardware/sistema**: POCO X5 Pro 5G (`redwood`, SM7325), anche QEMU. Userland iOS 16.5 (20F66) dell'iPhone X,
  `xnu-8796.121.2`. Rete via Wi-Fi.
- **Licenza**: kernel APSL 2.0; caricatore, userland, strumenti, documentazione MIT (`LICENSE`).
- **Autori da accreditare**: **vcvkk** (lavoro e diario sull'App Store, ramo `services`, 19–20 ottobre 2026);
  **shimoku** / OOSEX GANG (manutenzione e rimozione). Org [`oosexgang`](https://github.com/oosexgang/hakios-xnu-kernel).
- **Cosa è stato osservato** (commit e diario `docs/ru/notes.md`, letto a `63fc2b4`):
  - L'App Store si apre e, dopo l'onboarding, **disegna la propria interfaccia**. È `JetEngine` che rende il documento di
    interfaccia scaricato da Apple. Prove: `appstore-welcome.png`, `appstore-ads.png`, `appstore-tabs.png`
    (commit `5154263239`, `182ead41b7`).
  - **Non** è caduto su FairPlay né su un accesso: il negozio non apre nemmeno una connessione di rete per i contenuti.
    Il blocco è interno a `AppleMediaServices`: manca la «storefront» (la regione), che su un dispositivo vero viene
    dall'account o dalla SIM (via CommCenter, che qui va in crash-loop per driver IOKit assenti).
  - Il diario attribuisce la scheda Today vuota al demone `fairplayd.H2`, che si avvia e **esce subito** perché gli manca
    il driver FairPlay nel kernel; la richiesta dei contenuti attende una firma che non arriva.
- **Livello raggiunto: L1.** Nessun contenuto, nessun accesso nel negozio, nessun download.
- **Stato pubblico**: gli esperimenti (le librerie iniettate in `appstored` e nel demone FairPlay, e uno strumento di
  prova) **sono stati tolti dal repository** il 26 ottobre (`698b648cb7`). Restano nella storia del ramo. La
  [revisione legale del progetto](https://github.com/oosexgang/hakios-xnu-kernel/blob/main/docs/legal-review.md)
  (`docs/legal-review.md`) chiede di tenerli fuori dal pubblico (punto «P0-c»).
- **Dichiarazione su Apple ID (non verificata)**: un messaggio del canale Telegram ufficiale (@oosexgangdevs,
  intorno al 3 novembre 2026) parla di «progressi sull'autenticazione Apple ID» con 2FA e token GrandSlam salvati, e
  aggiunge che «iCloud e App Store hanno ancora dei muri». È **solo testo, senza codice né immagini pubblicate**, e il
  README/`LEGAL.md` del repository dice il contrario («Apple's services … not supported and not a goal; nothing here
  makes a device pass as an iPhone to Apple's servers»). Da trattare come affermazione non confermata.

### 2.2 ios-sweet (dipa-studio) — «The App Store opens» (solo L0, dichiarato)

- Redmi Note 10 Pro, iOS 16.7.16 (20H392), **la nostra stessa base**. Licenza MIT.
- Il sito (`site/projects.html`, commit `45263bee51`, 23 ottobre 2026) scrive: «The App Store opens (installing apps is
  untested, since there is no internet yet)». `site/future.html` aggiunge che download e accesso Apple ID «need Apple's
  FairPlay DRM and the Secure Enclave … maybe one that never fully works». Il README dice «There is no App Store».
  Nessuna foto o log a supporto del «opens»: **L0 dichiarato**.
- Molti fork ripetono la stessa frase o, come `Arming1/ios-sweet`, notano solo l'icona App Store a schermo e che
  `fairplayd.H2` esce subito.

### 2.3 IOS-26-Hot (nithinyadavamaraboina-online) — negozio proprio, non quello di Apple

- iOS 26.5.2 (arm64e) su SM8150 (Realme X2 Pro, Pixel 4). **Estende la base SWEET/ios-sweet**. Licenza MIT (letto a
  `d249939b`). 1 stella, spinto il 3 novembre 2026; molte affermazioni grosse e **non verificate** da terzi.
- Dichiara «App Store & 3rd-Party Apps: Working», ma la riga stessa spiega cos'è: **HotStore**, un negozio proprio che
  mostra le classifiche/ricerca tramite l'API pubblica `itunes.apple.com/lookup|search`, un catalogo di `.ipa`
  **non cifrate** da repository tipo AltStore/SideStore, un punto di caricamento `.ipa` via Wi‑Fi, e un impacchettatore
  di WebApp. L'installatore (`hotstored.m`) **rifiuta esplicitamente** i binari con `cryptid != 0` («Encrypted App Store
  binary (FairPlay cryptid=1) … import an unencrypted (cryptid=0) .ipa»). C'è anche **HotAuth** («microG for iOS»), che
  prova a usare un server *anisette* esterno per Apple ID: dichiarato, non dimostrato. Nelle note dichiara **non
  funzionante**: «FairPlay-encrypted App Store binaries».
- In pratica: è il modello «sideload di `.ipa` non cifrate» (sezione 4), vestito da negozio. Non è l'App Store di Apple.

### 2.4 ios-flame (NWylynko) — sideload di `.ipa` decifrata, senza `installd`

- Pixel 4, SM8150 + Adreno 640, base ios-sweet, iOS 16.7.16. Licenza MIT (patch XNU APSL 2.0). È **già nei nostri
  crediti** per CPU e GPU.
- `HANDOFF.md`: ha fatto girare **YouTube** da una `.ipa` **decifrata** e firmata ad-hoc. Senza `installd`: scompatta la
  `.app` in `/Applications` e ricostruisce LaunchServices (`lsreg -r`). Ha tolto `fairplayd.H2` da launchd perché, non
  trovando il driver, mandava in stallo `locationd`. Note utili anche per noi: patch kernel AMFI
  (`0103-flame-amfi-trust-query`, `0114-flame-amfi-dyld-policy`) e `clonefile` (`0100`).

### 2.5 ios-apollo / project-BadApple (aTechGeekDev) — sideload esplicito

- Mi 10T (`apollo`), base ios-sweet/SWEET, iOS 16.7.16. Licenza MIT.
- README esplicito: «App Store apps are encrypted (FairPlay); this port cannot decrypt them — Not started». «Install
  your own apps: Decrypted or self-built … TrollStore needs a CoreTrust bug that iOS 16.7.16 does not have». App copiate
  in `/Applications` e registrate al boot (`tools/app-pack.sh`, `jb-post.txt`).

### 2.6 Redmi K90 / `annibale` (iola1999) — HakiOS su iOS 26, sideload di `.ipa` decifrate

- SM8750, iOS 26.6.2, base HakiOS. Nessuna licenza dichiarata (**solo riferimento, niente copia di codice**).
- Elenca «installa ed esegue app `.ipa` già decifrate» tra ciò che funziona, e «App Store» tra ciò che non funziona.

### 2.7 Bergamota / Chlorophyll (ry-diffusion) — sideload, TrollStore nell'immagine

- Poco F1, iOS 18.7.8, controllo da macOS. Nessuna licenza dichiarata (**solo riferimento**).
- L'immagine include Procursus + Sileo + **TrollStore** e una via di sideload dal Mac. Nessuna affermazione sul negozio
  di Apple.

### 2.8 UniXNU (iPadOS) e altri

- UniXNU (iPadOS 17 su `sheng`, Snapdragon 8 Gen 2; org UniXNU) **non** dichiara App Store/Apple ID; il README nota «no
  SEP → no Apple ID». Documenta l'identità MobileGestalt via ADT (`docs/guide/mobilegestalt.md`), usata per far
  corrispondere userland e identità del dispositivo «donatore» (iPad7,1). Il port QEMU (xVanTuring/UniXNU-qemu) nota che
  **Setup si blocca** su `lockdownd` in attesa di `fairplayd`.
- ios-topaz (MakrSas), ios-sapphire (Angelpro09xd), ios-mh2lm (shenli99): nessuno dichiara App Store; `fairplayd.H2` in
  crash-loop per hardware assente. Già nei nostri crediti per Wi-Fi/ricarica.

### 2.9 Fuori scena: emulatori e Corellium (perché non aiutano)

- **Corellium** (iPhone virtuali per ricerca): la sua documentazione dice chiaramente che le macchine virtuali **non**
  abilitano l'App Store né l'accesso ai servizi Apple, e **non** eseguono app cifrate FairPlay; si carica solo una
  versione «signed, unencrypted» dell'app. È lo stesso limite che hanno questi port.
- **qemu-t8030 / ChefKiss (Inferno)**: emulano iPhone 11/iOS 14; arrivano attorno a SpringBoard. Nessun App Store.
- **vphone-cli / super-tart (vphone600ap)**: fanno girare iOS/iPadOS in VM **solo su Mac Apple Silicon**, usando i
  componenti «iPhone Research Environment» del firmware PCC di Apple — quindi con l'hardware virtuale (e il relativo
  driver FairPlay VirtIO) fornito da Apple stessa. Le note del progetto riescono a riscaricare le **app Apple
  preinstallate** e a installare un'app da Xcode con `installd`, ma dichiarano esplicitamente **non** dimostrati
  «Apple Account sign-in» né l'autorizzazione FairPlay di app di terzi. Non è una via replicabile su un Qualcomm.

---

## 3. Il muro tecnico: perché le app cifrate non girano (dal codice aperto di XNU)

Nel sorgente pubblico `xnu-8796.141.3` (quello che compiliamo), `bsd/kern/mach_loader.c` gestisce il comando di carico
`LC_ENCRYPTION_INFO(_64)`:

- `cryptid == 0` → app non cifrata, caricamento normale.
- `cryptid != 0` → serve un **decifratore**, registrato nel kernel tramite l'hook `text_crypter_create`
  (`osfmk/kern/page_decrypt.c`). Se nessuno lo ha registrato, `set_code_unprotect()` ritorna `LOAD_FAILURE`; se il
  decifratore c'è ma non ha le chiavi, ritorna `LOAD_DECRYPTFAIL` e il processo viene **ucciso** con motivo
  `EXEC_EXIT_REASON_FAIRPLAY_DECRYPT`.

Chi registra quell'hook è il kext FairPlay di Apple (`com.apple.driver.FairPlayIOKit`, `fairplayd`), che **non è
open source** e si appoggia a chiavi legate all'hardware Apple (Secure Enclave / GID). Di conseguenza, su questi port:

- `fairplayd.H2` si avvia e **esce subito** (lo osservano HakiOS, ios-flame, ios-apollo, ios-topaz, …), perché non trova
  il driver. Tutti lo tolgono da launchd per non mandare in stallo altri demoni.
- Le app dell'App Store, che sono sempre `cryptid = 1`, **non possono essere caricate**. Le app non cifrate
  (`cryptid = 0`) sì, perché la verifica delle firme in questi port è resa permissiva (stub AMFI; es. ios-sweet
  `0025/0035/0036`, ios-flame `0103/0114`).

Questa è la ragione per cui **tutti** i casi di «app funzionante» sono app **non cifrate**, e perché nessuno esegue
app cifrate: non è una questione di rete o di account, ma del decifratore e delle sue chiavi, assenti senza hardware
Apple. Non risulta alcun metodo pubblico per aggirarlo su hardware non Apple.

---

## 4. L'unica strada che funziona altrove: app non cifrate nel filesystem

Il modello comune (ios-flame, IOS-26-Hot, ios-apollo, K90, danhol86, Bergamota) è:

1. Partire da una `.app`/`.ipa` con codice arm64(e) **non cifrato**: compilata da sé, open source, o fornita già
   decifrata da chi la sviluppa.
2. Rendere permissive AMFI e firma del codice (già fatto nella nostra base), firmare ad-hoc.
3. Copiare la `.app` in `/Applications` (o in un contenitore sul volume dati) e **registrarla in LaunchServices** (come
   fa `uicache`/`lsreg -r`), senza passare da `installd`/App Store.

È esattamente ciò che il nostro [ROADMAP, «Installare app (.ipa)»](../../ROADMAP.md), già prevede. TrollStore **non** è
disponibile su iOS 16.7.16 (richiede bug di CoreTrust corretti da tempo); resta il percorso «copia + LaunchServices».

---

## 5. Analisi di fattibilità per `andromeda`, in ordine

1. **Rete prima di tutto.** Il nostro kernel ha già `ue0` (CDC-ECM) a `192.168.7.2/24` e lo stato del link per configd
   (patch 0075/0076/0080/0081). Manca il NAT lato PC (già documentato in
   [wifi-bluetooth.md §9](wifi-bluetooth.md)) e un servizio che configd veda in automatico. Senza rete non si prova
   nemmeno l'apertura dell'interfaccia del negozio. **Quick win, da fare comunque.**
2. **Orologio reale.** `appstored`/TLS e la verifica dei certificati vogliono un'ora plausibile. Oggi non abbiamo RTC;
   ios-flame e ios-apollo leggono l'RTC del PM8150 via SPMI (ios-flame kernel 0099, ios-apollo `apollo_rtc`): riferimento
   utile, da valutare quando serve.
3. **Attivazione/identità.** Già oggi usiamo la via nativa «Hactivation» (stato Activated senza record, via proprietà di
   sicurezza nel `/chosen` dell'ADT, `scripts/mkadt.py` + `scripts/sweet-root-tweaks.py`), con ECID **sintetico**
   (`SWEET_ECID`, non quello del telefono). Questo basta per SpringBoard e per app non cifrate. **Non** proseguiamo
   verso identità reali o record di attivazione (vedi sezione 8).
4. **App non cifrate (obiettivo realistico).** Dopo rete + orologio: provare a installare un'app arm64 non cifrata con
   il percorso della sezione 4, con GPU assente (le app Metal non disegnano finché non c'è la GPU). Riusabile: le note
   AMFI/`clonefile` e lo schema di registrazione di ios-flame (MIT) e di ios-apollo (MIT).
5. **Interfaccia del negozio (opzionale, sconsigliato).** Tecnicamente HakiOS ha mostrato che l'interfaccia può aprirsi
   via Wi-Fi, ma: (a) serve rete vera verso Internet, non solo CDC-ECM verso il PC; (b) non porta a contenuti, accesso
   né download; (c) HakiOS stessa l'ha tolta dal pubblico per i motivi legali della sezione 8. **Raccomando di non
   perseguirla.**
6. **Contenuti / accesso / download / app cifrate.** Non fattibili senza hardware Apple. Nessun progetto ci è arrivato.

**Cosa si può riusare (licenza permettendo) vs solo consultare**

- **Riusabile** (MIT/APSL, con credito): note e patch AMFI/`clonefile`/registrazione app di **ios-flame** e
  **ios-apollo**; schema di installazione «copia + LaunchServices». XNU è APSL 2.0.
- **Solo riferimento, niente copia di codice** (nessuna licenza dichiarata): IOS-26-Hot (HotStore/HotAuth), Redmi K90,
  Bergamota, ios-topaz, ios-sapphire, ios-mh2lm. Se mai servisse del loro codice, chiedere prima il permesso agli
  autori, come già per Wi-Fi/ricarica.
- **Da non riusare**: gli esperimenti HakiOS su `appstored`/FairPlay (rimossi dal pubblico, in zona grigia legale).

---

## 6. Il «sono sicuro di aver visto progetti che aprono l'App Store»

È vero, ed è coerente con **HakiOS** (19–20 ottobre 2026): l'App Store si apre e disegna la sua interfaccia (3
screenshot). Quasi certamente il proprietario ha visto quelle immagini o un ripost. Due precisazioni importanti:

- **«Si apre» = L1**, non il negozio funzionante: la scheda Today non si carica, non c'è accesso né download.
- Quelle immagini oggi sopravvivono soprattutto nel README di tanti fork (`docs/appstore-tabs.png`); gli esperimenti che
  le hanno prodotte **non sono più nel repository pubblico**.

---

## 7. Tabella di sintesi

| Progetto | HW / iOS | Livello App Store | Prova | FairPlay/cifrate | Licenza | Credito |
|---|---|---|---|---|---|---|
| HakiOS | SM7325 / 16.5 | **L1** (interfaccia disegnata) | 3 screenshot + log | no (fairplayd esce) | kernel APSL 2.0, resto MIT | vcvkk, shimoku, OOSEX GANG |
| ios-sweet | SM7150 / 16.7.16 | L0 (dichiarato) | solo testo | no | MIT | dipa-studio |
| IOS-26-Hot | SM8150 / 26.5.2 | negozio **proprio** (HotStore) | codice | rifiuta cryptid=1 | MIT | Nithin Amaraboina |
| ios-flame | SM8150 / 16.7.16 | sideload non cifrato (YouTube) | log + codice | no | MIT (XNU APSL) | NWylynko |
| ios-apollo | SM8150 / 16.7.16 | sideload non cifrato | README + codice | no (dichiarato) | MIT | aTechGeekDev |
| Redmi K90 | SM8750 / 26.6.2 | sideload `.ipa` decifrate | README | no | nessuna (riferim.) | iola1999 |
| Bergamota | SDM845 / 18.7.8 | sideload (TrollStore) | README | no | nessuna (riferim.) | ry-diffusion |
| Corellium | VM | **nessuno** (per policy) | doc ufficiale | no | commerciale | — |
| vphone-cli | **Mac Apple Silicon** | riscarica app Apple; no 3rd-party/sign-in | doc | FairPlay via VirtIO del Mac | MIT | Lakr233 |

---

## 8. Note legali e di ToS da segnalare

Queste sono cose da **evitare**, in linea con [CONTRIBUTING.md](../../CONTRIBUTING.md) («non faremo nulla che impersoni
un dispositivo reale o che violi»). Non sono un parere legale.

- **Falsificare l'identità di un dispositivo reale** (numero di serie, UDID, MLB, ECID di un iPhone esistente) per
  passare per quel dispositivo ai server Apple: da **non fare**. Usiamo già un ECID **sintetico** e un'attivazione
  offline, senza contattare i server Apple. La stessa revisione legale di HakiOS segnala come problema i valori di
  identità fissi scritti dal loro loader.
- **Accesso con Apple ID su hardware non Apple**: la guida Hackintosh (Dortania/OpenCore) avverte da anni di **non usare
  seriali reali** per i servizi Apple; Apple valida l'identità lato server e può **bloccare o limitare l'account**. I
  provider *anisette* (usati da AltStore/SideStore, e che HotAuth di IOS-26-Hot prova a richiamare) sono una dipendenza
  di terzi e una fonte documentata di errori e blocchi temporanei di accesso. **Non** useremo Apple ID personali su
  `andromeda`.
- **DRM FairPlay**: decifrare o aggirare la protezione delle app dell'App Store ricade sotto le norme anti-elusione
  (es. DMCA §1201). Il nostro percorso è solo app **non cifrate**; non implementiamo né aggiriamo FairPlay (il kernel
  si limita a *non* fornire il decifratore, non a romperlo).
- **Licenza d'uso di iOS**: consente l'esecuzione solo su dispositivi Apple; questo è un progetto di ricerca personale e
  non ridistribuiamo software Apple (già nel [CREDITS](../../CREDITS.md) e in CONTRIBUTING).
- **Toccare i processi del negozio e del DRM** e inviare richieste ai server Apple con un'identità inventata è proprio ciò
  che OOSEX GANG ha **rimosso** dal proprio repository pubblico su parere legale: un precedente da seguire.

---

## 9. Fonti

Tutte consultate il 6 novembre 2026 (sola lettura). I progetti senza licenza dichiarata sono citati solo come
riferimento; non ne copiamo codice senza permesso degli autori.

**HakiOS (OOSEX GANG)** — [`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) (kernel APSL
2.0, resto MIT), letto a `63fc2b4633bac3af8e57dadab9f5a8d4cb940438`. Autori: **vcvkk**, **shimoku**, OOSEX GANG.
- Diario: `docs/ru/notes.md` (in russo). `README.md`, `LEGAL.md`, `LICENSE`, `docs/legal-review.md`.
- Commit sull'App Store (autore vcvkk salvo nota): `d58e006a3e` «The App Store opens, and stops at the storefront»;
  `5154263239` e `182ead41b7` «The App Store draws … welcome card, the ads choice, and its own tab bar» (+ i tre PNG
  `docs/appstore-welcome.png`, `docs/appstore-ads.png`, `docs/appstore-tabs.png`); `9ecc07a7f1`/`045c8c4550` «… is
  fairplayd: 26 deaths …»; `b4c1708106` «Merge services into main»; **`698b648cb7`** (shimoku) «drop the App Store and
  FairPlay experiments».
- Canale Telegram ufficiale @oosexgangdevs (post ~3 novembre 2026, su Apple ID/2FA/GrandSlam): `https://t.me/s/oosexgangdevs`
  — affermazione non verificata.
- Sito terzo non ufficiale `https://hackios.net/en` (dichiara di non essere affiliato a OOSEX GANG né a Apple).

**ios-sweet** — [`dipa-studio/ios-sweet`](https://github.com/dipa-studio/ios-sweet) (MIT). `site/projects.html`,
`site/future.html`, `README.md`, commit `45263bee51df4a0f55cdf9c649af886399488ddb` (23 ottobre 2026). Autore: dipa-studio.

**IOS-26-Hot** — [`nithinyadavamaraboina-online/IOS-26-Hot`](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot)
(MIT), letto a `d249939b9c36380afaee1a76cf167aeee8347475`. Autore: **Nithin Amaraboina**. File letti: `README.md`,
`phone/store/HotStore.m`, `phone/store/hotstored.m` (rifiuto `cryptid=1` alle righe ~899–941), `phone/store/inject_appfix.m`,
`phone/auth/hotauthd.m`.

**ios-flame** — [`NWylynko/ios-flame`](https://github.com/NWylynko/ios-flame) (MIT; patch XNU APSL 2.0), letto a
`745c4341af86089efb0df851fa8a27fb8face624`. `HANDOFF.md` (YouTube, `fairplayd` rimosso, AMFI/clonefile). Autore:
**Nick Wylynko**. Già nei [CREDITS](../../CREDITS.md).

**ios-apollo / project-BadApple** — [`aTechGeekDev/ios-apollo`](https://github.com/aTechGeekDev/ios-apollo) (MIT),
`README.md` e note. Autore: **aTechGeekDev**.

**Redmi K90 / annibale** — [`iola1999/hackios-redmi-k90-annibale`](https://github.com/iola1999/hackios-redmi-k90-annibale)
(nessuna licenza dichiarata). `README.md`. Solo riferimento.

**Bergamota / Chlorophyll** — [`ry-diffusion/Bergamota`](https://github.com/ry-diffusion/Bergamota),
[`ry-diffusion/Chlorophyll`](https://github.com/ry-diffusion/Chlorophyll) (nessuna licenza dichiarata). `README.md`.
Solo riferimento.

**UniXNU** — [`UniXNU/UniXNU`](https://github.com/UniXNU/UniXNU), letto a `10edc1efeeabc8c180b1a3ad9f96b7c8e36a385e`.
`docs/guide/mobilegestalt.md`. [`xVanTuring/UniXNU-qemu`](https://github.com/xVanTuring/UniXNU-qemu) (`README.md`, nota
su `lockdownd`/`fairplayd` in Setup). Porto Surface Duo iOS 26 di **SmuOS** (`@none24smu`,
[`none24smu/UniXNU-zeta`](https://github.com/none24smu/UniXNU-zeta)); copertura: PC-WELT/TechAdvisor (29 ottobre 2026),
Windows Latest (27 ottobre 2026).

**ios-topaz / ios-sapphire / ios-mh2lm** — [`MakrSas/ios-topaz`](https://github.com/MakrSas/ios-topaz),
[`Angelpro09xd/ios-sapphire`](https://github.com/Angelpro09xd/ios-sapphire),
[`shenli99/ios-mh2lm`](https://github.com/shenli99/ios-mh2lm) (nessuna licenza dichiarata). `README.md`. Già in
[CREDITS](../../CREDITS.md).

**danhol86/ios-sweet** — [`danhol86/ios-sweet`](https://github.com/danhol86/ios-sweet) (MIT), `README.md` (strumento di
installazione di `.ipa` non cifrate; «does not decrypt App Store DRM»).

**XNU (Apple, APSL 2.0)** — [`apple-oss-distributions/xnu`](https://github.com/apple-oss-distributions/xnu), ref
`xnu-8796.141.3`: `bsd/kern/mach_loader.c` (`set_code_unprotect`, `LC_ENCRYPTION_INFO`, `LOAD_DECRYPTFAIL`,
`EXEC_EXIT_REASON_FAIRPLAY_DECRYPT`), `osfmk/kern/page_decrypt.c`/`.h` (hook `text_crypter_create`).

**Corellium** — documentazione «Testing iOS Apps» (`support.corellium.com/features/apps/testing-ios-apps`): App Store
non abilitato, nessuna app FairPlay, solo app non cifrate.

**Emulatori / VM** — [`TrungNguyen1909/qemu-t8030`](https://github.com/TrungNguyen1909/qemu-t8030) (archiviato 2023);
ChefKiss Inferno / [`ChefKissInc/QEMUAppleSilicon`](https://github.com/ChefKissInc/QEMUAppleSilicon);
[`Lakr233/vphone-cli`](https://github.com/Lakr233/vphone-cli) (MIT, letto a `a78483a30430b7b3ecdf51536881fd97a440da81`:
`Research/Guest/post_setup_signin_and_appstore.md`, `Research/Guest/installd_and_store_download_repair.md`,
`Research/Kernel/kernel_fairplay_kexts.md`); wh1te4ever `super-tart-vphone-writeup` (vphone600ap, febbraio 2026).

**Contesto Apple ID / anisette / Hackintosh** — documentazione AltStore/SideStore e discussioni della community sui
provider anisette e sugli errori/blocchi di accesso; guida Hackintosh Dortania/OpenCore sul non usare seriali reali per
i servizi Apple. (Consultate per i rischi della sezione 8.)

**Stampa** (contesto generale, nessuna conferma di App Store funzionante): Decrypt, Android Authority, Gizchina,
GIGAZINE, moreread.net (23–29 ottobre 2026).
