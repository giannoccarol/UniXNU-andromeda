# Programmi per iOS sul telefono

Ogni cartella `phone/<nome>/` contiene un programma che gira dentro iOS (demone o strumento), con due script:

| Script | Dove gira | Cosa fa |
|---|---|---|
| `phone/<nome>/build.sh [OUT]` (oppure `OUT=…` nell'ambiente) | runner macOS di GitHub con Xcode | compila per iOS 16 arm64 (`xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0`), firma ad hoc (`codesign -s -`, con gli entitlement se servono) e scrive i file in `$OUT/<nome>/` |
| `phone/<nome>/install-root.sh ROOT OUTDIR` | PC Linux, come root, durante la costruzione del sistema iOS | copia i file compilati (`OUTDIR/<nome>/`) nel sistema iOS montato in `ROOT` ed eventualmente aggiunge il job di launchd in `ROOT/System/Library/xpc/launchd.plist` |

Regole:

- Un workflow esegue ogni `phone/*/build.sh`; un unico aggancio nella costruzione del sistema esegue ogni
  `phone/*/install-root.sh`. Gli script non toccano `scripts/rebuild-root.sh`, `scripts/sweet-root-tweaks.py` né i
  workflow.
- `install-root.sh` è idempotente; se il programma non è stato compilato avvisa ed esce con 0, senza cambiare il
  sistema.
- I binari vanno in `/private/var/local/bin/`; i job di launchd si chiamano `org.andromeda.<nome>`.
- Ogni programma resta inattivo se manca il suo parametro di avvio del kernel ([CONTRIBUTING.md](../CONTRIBUTING.md) §4).
- Nessun file di iOS (IPSW, librerie, device tree) nel codice o negli output; solo l'SDK di Xcode.
- I dati del telefono (`phone/<seriale>/`, `phone/logs/`) restano fuori da git.
- Crediti: ogni codice o idea ripresa va in [CREDITS.md](../CREDITS.md) nello stesso cambiamento.

Programmi:

- [`andromedad/`](andromedad/): apre l'app Fotocamera quando si apre lo slider (patch del kernel 0097,
  `andrslider=1 andrslidercam=1`).
- [`andromeda-audio/`](andromeda-audio/): plug-in `AndromedaAudio.driver` per l'audio server di iOS (fase A0 di
  `docs/research/audio.md`): dispositivi dell'iPhone X senza hardware, uscita scartata, ingresso silenzioso; si installa in
  `/System/Library/Audio/Plug-Ins/HAL` e rimette `VirtualAudio.plugin` al suo posto. È l'unico programma senza parametro di
  avvio: senza di lui VirtualAudio va tenuto disattivato (`andraudiorcv=1` = esperimento del ricevitore).
- [`turnip/`](turnip/) e [`metal2/`](metal2/): il driver Vulkan Turnip di Mesa e il plug-in Metal `SweetMetal2.bundle`
  (fase F5 di `docs/research/gpu.md`, da ios-flame e UniXNU). Si compilano solo con `ANDROMEDA_GPU_USERLAND=1`, nel
  workflow `gpu-userland-andromeda` (lungo); l'artifact va unito in `inputs/andromeda/userland/`. Le librerie vanno in
  `/private/var/local/lib/`, il bundle in `/System/Library/Extensions/`; niente si avvia da solo.
- [`andromeda-apps/`](andromeda-apps/): copia in `/Applications` le app non cifrate dell'utente preparate sul PC con
  `scripts/add-ipa.py` (`inputs/andromeda/apps/`, mai nel repository); con `andrapps=1|2` controlla al boot che
  LaunchServices le conosca (`2` ricostruisce il database; `andrappsopen=ID` apre un'app senza touch).
