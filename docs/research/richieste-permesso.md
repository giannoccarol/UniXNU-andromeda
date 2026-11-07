# Richieste di permesso agli autori

| Progetto | Stato (6 novembre 2026) |
|---|---|
| `nomorecoolnicknames/hakios-sm8150` | **inviata**: [issue #1](https://github.com/nomorecoolnicknames/hakios-sm8150/issues/1) |
| `MakrSas/ios-topaz` | issue e Discussions disattivate; l'unico contatto indicato è X (`makrotasuss`): messaggio da inviare dal proprietario del progetto (testo breve sotto) |

Due progetti da cui vorremmo riprendere codice non dichiarano una licenza: senza permesso possiamo solo usarli come
riferimento e riscrivere. Bozze delle issue da aprire sui loro repository (in inglese, come i loro progetti).

---

## 1. `MakrSas/ios-topaz`: Wi-Fi (patch 0113-0130, 0900)

**Titolo:** Permission / license question: reusing the TopazModem Wi-Fi patches in an SM8150 port

> Hi, and thank you for ios-topaz: getting Wi-Fi (scan, WPA2 join, Apple80211 for wifid) working on the ios-sweet base is
> impressive work.
>
> I am porting the same base (ios-sweet, iOS 16.7.16 / 20H392) to the Xiaomi Mi Mix 3 5G (SM8150, WCN3990-family Wi-Fi on
> the modem DSP), forked from the Mi 9 UniXNU port: https://github.com/giannoccarol/UniXNU-andromeda (branch `andromeda`).
> Your patches 0113-0130 and 0900 are the closest working reference for our Wi-Fi.
>
> The repository does not state a license, so I would like to ask before reusing any of it: would you be OK with us
> adapting those patches (with full credit to you in our CREDITS.md and in the file headers), and if so, under which
> license? If you prefer we only use them as a reference and write our own code, we will do that.
>
> Thanks either way!

---

**Versione breve per X (messaggio diretto):**

> Hi Makr, thanks for ios-topaz! I'm porting the same ios-sweet / iOS 16.7.16 base to the Xiaomi Mi Mix 3 5G (SM8150):
> github.com/giannoccarol/UniXNU-andromeda (branch andromeda). Your Wi-Fi patches (0113-0130, 0900) are the best
> reference for our WCN3990. The repo has no license: may we adapt them, with full credit? If not, we'll only use them
> as a reference and write our own. Thanks!

---

## 2. `nomorecoolnicknames/hakios-sm8150`: avvio del modem e Wi-Fi/BT su SM8150

**Titolo:** Permission / license question: reusing the SM8150 modem/PAS and BT bring-up code

> Hi, and thank you for hakios-sm8150: it is the only public XNU work on SM8150 that boots the modem through TrustZone and
> talks to WLFW and the WCN399x Bluetooth UART.
>
> I am porting iOS 16 on the ios-sweet/UniXNU base to the Xiaomi Mi Mix 3 5G (SM8150, andromeda):
> https://github.com/giannoccarol/UniXNU-andromeda (branch `andromeda`). The SoC-side code in `vayu_pas.c`, `vayu_modem.c`,
> `vayu_rmtfs.c` and `vayu_bt.c` would save us a lot of reverse work.
>
> The repository does not state a license, so I would like to ask: may we adapt those files (with full credit to you in
> CREDITS.md and in the file headers), and under which license? If you prefer we only use them as a reference, we will
> write our own.
>
> Thanks!

---

Mentre aspettiamo le risposte, quei progetti si usano **solo come riferimento** (fatti hardware, sequenze, indirizzi);
il codice si scrive da zero o si prende da fonti con licenza compatibile (HakiOS principale sotto APSL, `renoir` secondo
il suo `NOTICE.md`, documentazione e driver Linux come riferimento per i protocolli).
