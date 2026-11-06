# Regole del progetto

## 1. Crediti, sempre

Questo port è costruito quasi tutto sul lavoro di altri. **Non presentiamo mai come nostro il lavoro di qualcun altro.**

- Ogni volta che si riprende codice, configurazioni, patch, misure, documentazione o idee da un altro progetto o da una
  persona, si aggiunge (o si aggiorna) la voce in [CREDITS.md](CREDITS.md) **nello stesso cambiamento**: autore
  (nome e/o handle), progetto con link, e cosa esattamente usiamo.
- I file copiati o adattati mantengono intestazioni, copyright e licenze originali; le licenze che lo richiedono vanno in
  `licenses/`.
- Nella documentazione e nei report si scrive chi ha fatto cosa: se un risultato dipende dal lavoro di un altro port (per
  esempio il Mi 9), lo si dice esplicitamente.
- Le correzioni che valgono anche per gli altri port vanno proposte a loro.
- I crediti stanno in `CREDITS.md`, nel README e nei report, **non nei messaggi di commit**.

## 2. Descrivere solo quello che si è visto

- Un componente "funziona" solo se è stato visto funzionare sul telefono; altrimenti è "compilato", "in prova" o
  "non funziona", con la prova (log, foto) citata.
- Errori e ipotesi sbagliate si correggono nella documentazione, non si nascondono.

## 3. Niente software Apple nel repository

- Né IPSW, né immagini del sistema iOS, né device tree estratti, né firmware del telefono: si scaricano o si estraggono
  in locale (`inputs/`, `out/`, `phone/`, esclusi da git).
- I workflow non pubblicano mai file derivati dall'IPSW.

## 4. Sicurezza del telefono

- Si scrive solo su `userdata` (e, quando deciso, su `boot`): bootloader, recovery, modem, `persist` e le altre
  partizioni non si toccano. I backup di IMEI e calibrazioni stanno in `phone/<seriale>/backup/`.
- Ogni driver nuovo parte spento e si accende con un suo parametro di avvio.
- Valori di tensione, corrente e frequenza mai oltre quelli del kernel Android del telefono.

## 5. Commit

- Messaggi di commit in inglese, senza righe di attribuzione di strumenti o assistenti AI e senza crediti (che stanno in
  `CREDITS.md`).
